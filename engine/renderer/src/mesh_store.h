#pragma once

#include <span>
#include <vector>

#include <nvrhi/nvrhi.h>

#include "foh/renderer/components.h"
#include "gpu_structs.h"

namespace foh {

struct MeshRange {
    u32 firstIndex = 0;
    u32 indexCount = 0;
    i32 baseVertex = 0;
};

// All mesh geometry in one vertex + one index buffer: the built-in primitives first,
// then imported meshes appended at runtime. Both buffers are created with
// keepInitialState, so any command list can use them without the render graph knowing
// about them. A "mesh id" names one draw range (an imported mesh has one per submesh).
//
// Growing replaces the buffers (the old ones stay alive until the GPU is done with
// them: NVRHI holds them through the command lists that use them). Space of released
// meshes is not reused yet; the ranges are, so ids stay small.
//
// Thread-safety: append/addRange/releaseRange on the main thread between frames only;
// the const accessors are safe from render jobs while no mutation happens.
class MeshStore {
public:
    static MeshStore create(nvrhi::IDevice* device);

    nvrhi::IBuffer* vertexBuffer() const { return m_vertices; }
    nvrhi::IBuffer* indexBuffer() const { return m_indices; }
    // Unknown ids fall back to the cube.
    const MeshRange& range(u32 mesh) const {
        return m_ranges[mesh < m_ranges.size() ? mesh : static_cast<u32>(BuiltinMesh::Cube)];
    }
    u32 rangeCount() const { return static_cast<u32>(m_ranges.size()); }

    // Where an appended block starts, for building ranges inside it.
    struct Block {
        u32 firstIndex = 0;
        i32 baseVertex = 0;
    };
    Block append(nvrhi::IDevice* device, std::span<const MeshVertex> vertices, std::span<const u32> indices);
    // Indices are relative to the block's first index; vertex indices are relative to its base vertex.
    u32 addRange(const MeshRange& range);
    void releaseRange(u32 id);

private:
    void upload(nvrhi::IDevice* device, size_t firstVertex, size_t vertexCount, size_t firstIndex, size_t indexCount);
    void grow(nvrhi::IDevice* device, size_t vertexCapacity, size_t indexCapacity);

    nvrhi::BufferHandle m_vertices;
    nvrhi::BufferHandle m_indices;
    std::vector<MeshVertex> m_cpuVertices; // CPU mirror: re-uploaded when the buffers grow
    std::vector<u32> m_cpuIndices;
    size_t m_vertexCapacity = 0;
    size_t m_indexCapacity = 0;
    std::vector<MeshRange> m_ranges;
    std::vector<u32> m_freeRanges;
};

} // namespace foh
