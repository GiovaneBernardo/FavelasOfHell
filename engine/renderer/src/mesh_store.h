#pragma once

#include <span>
#include <vector>

#include <nvrhi/nvrhi.h>

#include "foh/renderer/components.h"
#include "gpu_structs.h"

namespace foh {

inline constexpr u32 kNoMeshBlock = ~0u;

struct MeshRange {
    u32 firstIndex = 0;
    u32 indexCount = 0;
    i32 baseVertex = 0;
    // Skinned meshes (ADR 0009): all submeshes of one asset share the block's vertices, which the skinning pass
    // deforms as a whole, so a range knows how many there are and where the block starts (baseVertex).
    u32 vertexCount = 0;
    bool skinned = false;
    u32 block = kNoMeshBlock; // the vertex/index block the range lives in; freed when its last range is released
};

// Joint indices (4 x u8) and weights (4 x unorm8, summing to 255) of one vertex. Parallel to MeshVertex.
struct SkinInfluence {
    u32 joints = 0;
    u32 weights = 0;
};
static_assert(sizeof(SkinInfluence) == 8);

// All mesh geometry in one vertex + one index buffer: the built-in primitives first,
// then imported meshes appended at runtime. Both buffers are created with
// keepInitialState, so any command list can use them without the render graph knowing
// about them. A "mesh id" names one draw range (an imported mesh has one per submesh).
//
// Growing replaces the buffers (the old ones stay alive until the GPU is done with
// them: NVRHI holds them through the command lists that use them). Vertex/index space is
// handed out by a first-fit allocator and returns to it when the last range of a block is
// released (streamed terrain tiles and hot-reloaded meshes depend on this); range ids are reused too.
//
// Thread-safety: append/addRange/releaseRange on the main thread between frames only;
// the const accessors are safe from render jobs while no mutation happens.
class MeshStore {
public:
    static MeshStore create(nvrhi::IDevice* device);

    nvrhi::IBuffer* vertexBuffer() const { return m_vertices; }
    nvrhi::IBuffer* indexBuffer() const { return m_indices; }
    // One SkinInfluence per vertex of vertexBuffer() (zeros for static meshes), for the skinning pass.
    nvrhi::IBuffer* influenceBuffer() const { return m_influences; }
    // Unknown ids fall back to the cube.
    const MeshRange& range(u32 mesh) const {
        return m_ranges[mesh < m_ranges.size() ? mesh : static_cast<u32>(BuiltinMesh::Cube)];
    }
    u32 rangeCount() const { return static_cast<u32>(m_ranges.size()); }

    // Where an appended block starts, for building ranges inside it.
    struct Block {
        u32 firstIndex = 0;
        i32 baseVertex = 0;
        u32 id = kNoMeshBlock; // pass it in the MeshRange of every range cut from this block
    };
    // `influences` is empty (static mesh) or one entry per vertex.
    Block append(nvrhi::IDevice* device, std::span<const MeshVertex> vertices, std::span<const u32> indices,
                 std::span<const SkinInfluence> influences = {});
    // Indices are relative to the block's first index; vertex indices are relative to its base vertex.
    u32 addRange(const MeshRange& range);
    void releaseRange(u32 id);

private:
    void upload(nvrhi::IDevice* device, size_t firstVertex, size_t vertexCount, size_t firstIndex, size_t indexCount);
    void grow(nvrhi::IDevice* device, size_t vertexCapacity, size_t indexCapacity);

    struct Span {
        u32 start, size;
    };
    struct BlockInfo {
        u32 vertexStart = 0, vertexCount = 0, indexStart = 0, indexCount = 0, refs = 0;
        bool live = false;
    };
    static u32 allocateSpan(std::vector<Span>& freeList, u32& top, u32 count);
    static void freeSpan(std::vector<Span>& freeList, u32& top, u32 start, u32 count);

    nvrhi::BufferHandle m_vertices;
    nvrhi::BufferHandle m_indices;
    nvrhi::BufferHandle m_influences;
    std::vector<SkinInfluence> m_cpuInfluences; // always as long as m_cpuVertices
    std::vector<MeshVertex> m_cpuVertices; // CPU mirror: re-uploaded when the buffers grow
    std::vector<u32> m_cpuIndices;
    size_t m_vertexCapacity = 0;
    size_t m_indexCapacity = 0;
    std::vector<MeshRange> m_ranges;
    std::vector<u32> m_freeRanges;
    std::vector<Span> m_freeVertexSpans, m_freeIndexSpans; // sorted by start, coalesced
    u32 m_vertexTop = 0, m_indexTop = 0;                    // high-water marks of the two buffers
    std::vector<BlockInfo> m_blocks;
    std::vector<u32> m_freeBlockIds;
};

} // namespace foh
