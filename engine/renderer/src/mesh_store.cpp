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
        m.vertices.push_back({c - f.u * 0.5f - f.v * 0.5f, f.normal, {0, 1}});
        m.vertices.push_back({c + f.u * 0.5f - f.v * 0.5f, f.normal, {1, 1}});
        m.vertices.push_back({c + f.u * 0.5f + f.v * 0.5f, f.normal, {1, 0}});
        m.vertices.push_back({c - f.u * 0.5f + f.v * 0.5f, f.normal, {0, 0}});
        for (u32 i : {0u, 1u, 2u, 0u, 2u, 3u}) m.indices.push_back(base + i);
    }
    return m;
}

// Unit quad in the XZ plane facing +Y.
MeshData makePlane() {
    MeshData m;
    m.vertices = {{{-0.5f, 0, 0.5f}, {0, 1, 0}, {0, 1}},
                  {{0.5f, 0, 0.5f}, {0, 1, 0}, {1, 1}},
                  {{0.5f, 0, -0.5f}, {0, 1, 0}, {1, 0}},
                  {{-0.5f, 0, -0.5f}, {0, 1, 0}, {0, 0}}};
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
            m.vertices.push_back({n * 0.5f, n, {u, v}});
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

    std::vector<MeshVertex> vertices;
    std::vector<u32> indices;
    for (const MeshData& m : meshes) {
        store.m_ranges.push_back(
            {static_cast<u32>(indices.size()), static_cast<u32>(m.indices.size()), static_cast<i32>(vertices.size())});
        vertices.insert(vertices.end(), m.vertices.begin(), m.vertices.end());
        indices.insert(indices.end(), m.indices.begin(), m.indices.end());
    }

    nvrhi::BufferDesc vb;
    vb.byteSize = vertices.size() * sizeof(MeshVertex);
    vb.isVertexBuffer = true;
    vb.initialState = nvrhi::ResourceStates::VertexBuffer;
    vb.keepInitialState = true;
    vb.debugName = "BuiltinMeshVertices";
    store.m_vertices = device->createBuffer(vb);

    nvrhi::BufferDesc ib;
    ib.byteSize = indices.size() * sizeof(u32);
    ib.isIndexBuffer = true;
    ib.initialState = nvrhi::ResourceStates::IndexBuffer;
    ib.keepInitialState = true;
    ib.debugName = "BuiltinMeshIndices";
    store.m_indices = device->createBuffer(ib);

    nvrhi::CommandListParameters params;
    params.enableImmediateExecution = false;
    nvrhi::CommandListHandle cmd = device->createCommandList(params);
    cmd->open();
    cmd->writeBuffer(store.m_vertices, vertices.data(), vb.byteSize);
    cmd->writeBuffer(store.m_indices, indices.data(), ib.byteSize);
    cmd->close();
    device->executeCommandLists(&cmd, 1);
    return store;
}

} // namespace foh
