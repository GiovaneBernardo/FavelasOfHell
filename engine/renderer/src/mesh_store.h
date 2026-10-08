#pragma once

#include <vector>

#include <nvrhi/nvrhi.h>

#include "foh/renderer/components.h"

namespace foh {

struct MeshRange {
    u32 firstIndex = 0;
    u32 indexCount = 0;
    i32 baseVertex = 0;
};

// The built-in primitives in one vertex + one index buffer. Both are created
// with keepInitialState, so any command list can use them without the render
// graph knowing about them (they never change after creation).
//
// Thread-safety: immutable after create().
class MeshStore {
public:
    static MeshStore create(nvrhi::IDevice* device);

    nvrhi::IBuffer* vertexBuffer() const { return m_vertices; }
    nvrhi::IBuffer* indexBuffer() const { return m_indices; }
    // Unknown ids fall back to the cube.
    const MeshRange& range(u32 mesh) const {
        return m_ranges[mesh < m_ranges.size() ? mesh : static_cast<u32>(BuiltinMesh::Cube)];
    }

private:
    nvrhi::BufferHandle m_vertices;
    nvrhi::BufferHandle m_indices;
    std::vector<MeshRange> m_ranges;
};

} // namespace foh
