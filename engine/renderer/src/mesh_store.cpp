#include "mesh_store.h"

#include <algorithm>
#include <cmath>

#include "foh/log.h"
#include "gpu_structs.h"

namespace foh {

namespace {

struct MeshData {
    std::vector<MeshVertex> vertices;
    std::vector<u32> indices;
};

// Unit cube centered at the origin, outward normals, CCW winding seen from outside.
MeshData makeCube() {
    MeshData m;
    struct Face {
        Vec3 normal, u, v;
    };
    const Face faces[6] = {
        {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}},  {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}},
        {{0, 1, 0}, {1, 0, 0}, {0, 0, -1}},  {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}},
        {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}},   {{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}},
    };
    for (const Face& f : faces) {
        const u32 base = static_cast<u32>(m.vertices.size());
        const Vec3 c = f.normal * 0.5f;
        const PackedTangent t = PackTangent(Vec4(f.u, 1.0f));
        m.vertices.push_back({c - f.u * 0.5f - f.v * 0.5f, f.normal, {0, 1}, t});
        m.vertices.push_back({c + f.u * 0.5f - f.v * 0.5f, f.normal, {1, 1}, t});
        m.vertices.push_back({c + f.u * 0.5f + f.v * 0.5f, f.normal, {1, 0}, t});
        m.vertices.push_back({c - f.u * 0.5f + f.v * 0.5f, f.normal, {0, 0}, t});
        for (u32 i : {0u, 1u, 2u, 0u, 2u, 3u}) m.indices.push_back(base + i);
    }
    return m;
}

// Unit quad in the XZ plane facing +Y.
MeshData makePlane() {
    MeshData m;
    const PackedTangent t = PackTangent(Vec4(1, 0, 0, 1));
    m.vertices = {{{-0.5f, 0, 0.5f}, {0, 1, 0}, {0, 1}, t},
                  {{0.5f, 0, 0.5f}, {0, 1, 0}, {1, 1}, t},
                  {{0.5f, 0, -0.5f}, {0, 1, 0}, {1, 0}, t},
                  {{-0.5f, 0, -0.5f}, {0, 1, 0}, {0, 0}, t}};
    m.indices = {0, 1, 2, 0, 2, 3};
    return m;
}

// UV sphere of diameter 1.
MeshData makeSphere() {
    MeshData m;
    constexpr u32 kSegments = 32, kRings = 16;
    for (u32 r = 0; r <= kRings; ++r) {
        const f32 v = static_cast<f32>(r) / kRings;
        const f32 phi = v * math::kPi;
        for (u32 s = 0; s <= kSegments; ++s) {
            const f32 u = static_cast<f32>(s) / kSegments;
            const f32 theta = u * 2.0f * math::kPi;
            const Vec3 n{std::sin(phi) * std::cos(theta), std::cos(phi), std::sin(phi) * std::sin(theta)};
            m.vertices.push_back({n * 0.5f, n, {u, v}, PackTangent(Vec4(-std::sin(theta), 0.0f, std::cos(theta), 1.0f))});
        }
    }
    for (u32 r = 0; r < kRings; ++r) {
        for (u32 s = 0; s < kSegments; ++s) {
            const u32 a = r * (kSegments + 1) + s;
            const u32 b = a + kSegments + 1;
            for (u32 i : {a, a + 1, b, a + 1, b + 1, b}) m.indices.push_back(i);
        }
    }
    return m;
}

} // namespace

MeshStore MeshStore::create(nvrhi::IDevice* device) {
    MeshStore store;
    const MeshData meshes[] = {makeCube(), makePlane(), makeSphere()};
    static_assert(static_cast<u32>(BuiltinMesh::Count) == 3, "update the table above");

    // Room for imported meshes from the start, so the first imports do not reallocate.
    store.grow(device, 1u << 16, 1u << 18);
    for (const MeshData& m : meshes) {
        const Block b = store.append(device, m.vertices, m.indices);
        store.addRange({b.firstIndex, static_cast<u32>(m.indices.size()), b.baseVertex, 0, false, b.id});
    }
    return store;
}

void MeshStore::grow(nvrhi::IDevice* device, size_t vertexCapacity, size_t indexCapacity) {
    m_vertexCapacity = vertexCapacity;
    m_indexCapacity = indexCapacity;

    nvrhi::BufferDesc vb;
    vb.byteSize = vertexCapacity * sizeof(MeshVertex);
    vb.isVertexBuffer = true;
    vb.structStride = sizeof(MeshVertex); // the skinning pass reads it as a structured buffer
    vb.initialState = nvrhi::ResourceStates::VertexBuffer | nvrhi::ResourceStates::ShaderResource;
    vb.keepInitialState = true;
    vb.debugName = "MeshVertices";
    m_vertices = device->createBuffer(vb);

    nvrhi::BufferDesc ib;
    ib.byteSize = indexCapacity * sizeof(u32);
    ib.isIndexBuffer = true;
    ib.initialState = nvrhi::ResourceStates::IndexBuffer;
    ib.keepInitialState = true;
    ib.debugName = "MeshIndices";
    m_indices = device->createBuffer(ib);

    nvrhi::BufferDesc sb;
    sb.byteSize = vertexCapacity * sizeof(SkinInfluence);
    sb.structStride = sizeof(SkinInfluence);
    sb.initialState = nvrhi::ResourceStates::ShaderResource;
    sb.keepInitialState = true;
    sb.debugName = "MeshSkinInfluences";
    m_influences = device->createBuffer(sb);

    m_cpuVertices.resize(vertexCapacity);
    m_cpuInfluences.resize(vertexCapacity);
    m_cpuIndices.resize(indexCapacity);
    // The new buffers start empty: re-upload what the old ones held.
    if (m_vertexTop || m_indexTop) upload(device, 0, m_vertexTop, 0, m_indexTop);
}

void MeshStore::upload(nvrhi::IDevice* device, size_t firstVertex, size_t vertexCount, size_t firstIndex, size_t indexCount) {
    nvrhi::CommandListParameters params;
    params.enableImmediateExecution = false;
    nvrhi::CommandListHandle cmd = device->createCommandList(params);
    cmd->open();
    if (vertexCount)
        cmd->writeBuffer(m_vertices, m_cpuVertices.data() + firstVertex, vertexCount * sizeof(MeshVertex),
                         firstVertex * sizeof(MeshVertex));
    if (vertexCount)
        cmd->writeBuffer(m_influences, m_cpuInfluences.data() + firstVertex, vertexCount * sizeof(SkinInfluence),
                         firstVertex * sizeof(SkinInfluence));
    if (indexCount)
        cmd->writeBuffer(m_indices, m_cpuIndices.data() + firstIndex, indexCount * sizeof(u32), firstIndex * sizeof(u32));
    cmd->close();
    device->executeCommandLists(&cmd, 1);
}

u32 MeshStore::allocateSpan(std::vector<Span>& freeList, u32& top, u32 count) {
    for (size_t i = 0; i < freeList.size(); ++i) {
        if (freeList[i].size < count) continue;
        const u32 start = freeList[i].start;
        freeList[i].start += count;
        freeList[i].size -= count;
        if (freeList[i].size == 0) freeList.erase(freeList.begin() + static_cast<std::ptrdiff_t>(i));
        return start;
    }
    const u32 start = top;
    top += count;
    return start;
}

void MeshStore::freeSpan(std::vector<Span>& freeList, u32& top, u32 start, u32 count) {
    if (count == 0) return;
    size_t at = 0;
    while (at < freeList.size() && freeList[at].start < start) ++at;
    freeList.insert(freeList.begin() + static_cast<std::ptrdiff_t>(at), Span{start, count});
    // Coalesce with the next and the previous span.
    if (at + 1 < freeList.size() && freeList[at].start + freeList[at].size == freeList[at + 1].start) {
        freeList[at].size += freeList[at + 1].size;
        freeList.erase(freeList.begin() + static_cast<std::ptrdiff_t>(at) + 1);
    }
    if (at > 0 && freeList[at - 1].start + freeList[at - 1].size == freeList[at].start) {
        freeList[at - 1].size += freeList[at].size;
        freeList.erase(freeList.begin() + static_cast<std::ptrdiff_t>(at));
        --at;
    }
    // A free span touching the high-water mark just lowers it.
    if (freeList[at].start + freeList[at].size == top) {
        top = freeList[at].start;
        freeList.erase(freeList.begin() + static_cast<std::ptrdiff_t>(at));
    }
}

MeshStore::Block MeshStore::append(nvrhi::IDevice* device, std::span<const MeshVertex> vertices, std::span<const u32> indices,
                                   std::span<const SkinInfluence> influences) {
    const u32 vertexStart = allocateSpan(m_freeVertexSpans, m_vertexTop, static_cast<u32>(vertices.size()));
    const u32 indexStart = allocateSpan(m_freeIndexSpans, m_indexTop, static_cast<u32>(indices.size()));

    u32 blockId;
    if (!m_freeBlockIds.empty()) {
        blockId = m_freeBlockIds.back();
        m_freeBlockIds.pop_back();
    } else {
        blockId = static_cast<u32>(m_blocks.size());
        m_blocks.emplace_back();
    }
    m_blocks[blockId] = {vertexStart, static_cast<u32>(vertices.size()), indexStart, static_cast<u32>(indices.size()), 0, true};

    if (m_vertexTop > m_vertexCapacity || m_indexTop > m_indexCapacity) {
        size_t vc = m_vertexCapacity, ic = m_indexCapacity;
        while (vc < m_vertexTop) vc *= 2;
        while (ic < m_indexTop) ic *= 2;
        grow(device, vc, ic); // reallocates the CPU mirrors and re-uploads everything below the high-water marks
    }
    std::copy(vertices.begin(), vertices.end(), m_cpuVertices.begin() + vertexStart);
    if (influences.size() == vertices.size()) std::copy(influences.begin(), influences.end(), m_cpuInfluences.begin() + vertexStart);
    else std::fill_n(m_cpuInfluences.begin() + vertexStart, vertices.size(), SkinInfluence{});
    std::copy(indices.begin(), indices.end(), m_cpuIndices.begin() + indexStart);
    upload(device, vertexStart, vertices.size(), indexStart, indices.size());
    return Block{indexStart, static_cast<i32>(vertexStart), blockId};
}

u32 MeshStore::addRange(const MeshRange& range) {
    if (range.block < m_blocks.size()) ++m_blocks[range.block].refs;
    if (!m_freeRanges.empty()) {
        const u32 id = m_freeRanges.back();
        m_freeRanges.pop_back();
        m_ranges[id] = range;
        return id;
    }
    m_ranges.push_back(range);
    return static_cast<u32>(m_ranges.size() - 1);
}

void MeshStore::releaseRange(u32 id) {
    if (id < static_cast<u32>(BuiltinMesh::Count) || id >= m_ranges.size()) return; // builtins are permanent
    const u32 block = m_ranges[id].block;
    m_ranges[id] = {}; // indexCount 0: draws nothing
    m_freeRanges.push_back(id);
    if (block < m_blocks.size() && m_blocks[block].live && --m_blocks[block].refs == 0) {
        BlockInfo& b = m_blocks[block];
        freeSpan(m_freeVertexSpans, m_vertexTop, b.vertexStart, b.vertexCount);
        freeSpan(m_freeIndexSpans, m_indexTop, b.indexStart, b.indexCount);
        b.live = false;
        m_freeBlockIds.push_back(block);
    }
}

} // namespace foh
