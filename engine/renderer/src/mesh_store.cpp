#include "mesh_store.h"

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
        store.addRange({b.firstIndex, static_cast<u32>(m.indices.size()), b.baseVertex});
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

    // The new buffers start empty: re-upload what the old ones held.
    if (!m_cpuVertices.empty() || !m_cpuIndices.empty()) upload(device, 0, m_cpuVertices.size(), 0, m_cpuIndices.size());
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

MeshStore::Block MeshStore::append(nvrhi::IDevice* device, std::span<const MeshVertex> vertices, std::span<const u32> indices,
                                   std::span<const SkinInfluence> influences) {
    const Block block{static_cast<u32>(m_cpuIndices.size()), static_cast<i32>(m_cpuVertices.size())};
    m_cpuVertices.insert(m_cpuVertices.end(), vertices.begin(), vertices.end());
    if (influences.size() == vertices.size()) m_cpuInfluences.insert(m_cpuInfluences.end(), influences.begin(), influences.end());
    else m_cpuInfluences.resize(m_cpuVertices.size());
    m_cpuIndices.insert(m_cpuIndices.end(), indices.begin(), indices.end());

    if (m_cpuVertices.size() > m_vertexCapacity || m_cpuIndices.size() > m_indexCapacity) {
        size_t vc = m_vertexCapacity, ic = m_indexCapacity;
        while (vc < m_cpuVertices.size()) vc *= 2;
        while (ic < m_cpuIndices.size()) ic *= 2;
        grow(device, vc, ic); // uploads everything, including the new block
    } else {
        upload(device, static_cast<size_t>(block.baseVertex), vertices.size(), block.firstIndex, indices.size());
    }
    return block;
}

u32 MeshStore::addRange(const MeshRange& range) {
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
    m_ranges[id] = {}; // indexCount 0: draws nothing
    m_freeRanges.push_back(id);
}

} // namespace foh
